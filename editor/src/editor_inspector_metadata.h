// Declares the editor's semantic Inspector metadata: per-field display
// hints (name, category, tooltip, range/units, widget kind) and per-
// component display metadata, plus the pure Euler<->quaternion and layer-
// name helpers the metadata-driven widgets use. Presentation only: nothing
// here changes reflected field offsets/kinds or persisted storage layout.

#pragma once

#include <cstdint>

#include "engine/math/quat.h"
#include "engine/math/vec3.h"

namespace engine::editor {

/// Enumerates how the Inspector's generic reflected-field loop should draw
/// one field, layered over the raw core::TypeField::Kind. `Auto` keeps the
/// existing per-Kind default (plain InputFloat/InputScalar/Checkbox/...).
enum class InspectorWidget : std::uint8_t {
  Auto,
  Slider,
  Drag,
  Color,
  AngleDegrees, // a single radian float, edited/displayed in degrees
  EulerDegrees, // a Quat field, edited/displayed as pitch/yaw/roll degrees
  LayerMask,    // a Uint32 field, edited as named bit checkboxes
};

/// Semantic metadata for one reflected field of one component type. Looked
/// up by (typeName, fieldName) against core::TypeDescriptor's own strings so
/// the table stays keyed to the same identity the reflected-field loop
/// already walks; a miss falls back to Auto/no-range/no-units defaults, so
/// omitting a row never hides a field, only leaves it looking raw.
struct FieldMetadata final {
  const char *typeName = nullptr;
  const char *fieldName = nullptr;
  const char *displayName = nullptr;
  const char *category = "General";
  const char *tooltip = nullptr;
  const char *units = nullptr;
  float speed = 0.0F; // 0 => widget's own default drag/slider speed
  float min = 0.0F;
  float max = 0.0F; // min == max == 0 => unranged
  InspectorWidget widget = InspectorWidget::Auto;
  bool advanced = false; // hidden unless the Inspector's Advanced view is on
  bool readOnly = false;
};

/// Looks up field metadata for (typeName, fieldName); nullptr on a miss (the
/// caller falls back to Auto-widget defaults, never to hiding the field).
const FieldMetadata *find_field_metadata(const char *typeName,
                                         const char *fieldName) noexcept;

/// Semantic metadata for one persistent component type: how the Inspector
/// labels its section header and groups it in the Add Component menu.
struct ComponentMetadata final {
  const char *typeName = nullptr;
  const char *displayName = nullptr;
  const char *category = "General";
  const char *tooltip = nullptr;
};

/// Looks up component metadata by core::TypeDescriptor name
/// ("engine::runtime::Transform", ...); nullptr on a miss (the caller falls
/// back to the raw C++ type name so no component becomes unlabeled).
const ComponentMetadata *find_component_metadata(const char *typeName) noexcept;

/// Converts a rotation to pitch/yaw/roll degrees for display, using the same
/// axis convention as math::to_euler (pitch about +X, yaw about +Y, roll
/// about +Z; q = qy(yaw) * qx(pitch) * qz(roll)). Round-trip policy (issue
/// #156): storage stays the normalized quaternion; this is a presentation
/// projection recomputed fresh from the quaternion every call, not a staged
/// value, so it is exact for every orientation except the pitch = +-90 deg
/// gimbal case, where yaw and roll become linearly dependent and the
/// extraction picks yaw = 0 with roll absorbing the swing (math::to_euler's
/// documented behavior) -- a quat->degrees->quat round trip through that
/// pole reproduces the same rotation but not necessarily the same
/// (yaw, roll) split the user last typed. Editing away from the pole is
/// always a stable, exact round trip (verified in
/// editor_inspector_metadata_test.cpp).
math::Vec3 euler_degrees_from_quat(const math::Quat &rotation) noexcept;
/// Inverse of euler_degrees_from_quat; always returns a normalized
/// quaternion (from_euler composes three unit rotations).
math::Quat quat_from_euler_degrees(const math::Vec3 &degrees) noexcept;

/// Number of named collision-layer bit slots the LayerMask widget offers.
/// Named per-project layers are issue #163 scope (gameplay tags/named
/// collision layers); until that lands, the widget still replaces raw
/// integer entry with per-bit checkboxes labeled by slot index so the
/// author never has to compute a bitmask by hand.
inline constexpr std::size_t kInspectorLayerCount = 32U;
/// Returns the display label for collision-layer bit `index` ("Layer N"
/// until issue #163 adds a project-level name table); nullptr when index is
/// out of range.
const char *inspector_layer_name(std::uint32_t index) noexcept;

} // namespace engine::editor
